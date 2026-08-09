import { supabase } from '@/lib/supabase'

export default async function SupabaseTest() {
  const { data, error } = await supabase
    .from('positions')
    .select('*')
    .order('recorded_at', { ascending: false })
    .limit(1)

  if (error) {
    return <pre>{error.message}</pre>
  }

  return (
    <main style={{ padding: '40px' }}>
      <h1>Supabase Test</h1>
      <pre>{JSON.stringify(data, null, 2)}</pre>
    </main>
  )
}
